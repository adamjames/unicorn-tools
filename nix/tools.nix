# Host-side streaming tools (cosmic_cast, mocks, etc.)
{ pkgs, unstable }:

let
  # Python with all required packages for streaming tools
  pythonEnv = unstable.python313.withPackages (ps: with ps; [
    pip
    setuptools
    pyserial
    ptyprocess
    websockets
    pillow
    pygobject3
    dbus-python
    pygame  # for cosmic_mock.py
  ]);

  # Runtime libraries for ntrviewer-hr (dlopen'd by static SDL3)
  ntrviewerRuntimeLibs = with unstable; [
    libGL
    libxkbcommon
    wayland
    libx11
    libxcursor
    libxext
    libxi
    libxrandr
    libxfixes
    vulkan-loader
    libdecor
    libpulseaudio
    pipewire
  ];

  # NTRViewer-HR binary release
  ntrviewerVersion = "0.3.7.0";
  ntrviewerHr = unstable.stdenv.mkDerivation {
    pname = "ntrviewer-hr";
    version = ntrviewerVersion;

    src = pkgs.fetchurl {
      url = "https://github.com/xzn/ntrviewer-hr/releases/download/v${ntrviewerVersion}/NTRViewer-HR-Linux-x64.tar.gz";
      hash = "sha256-/hs8T738rs66DT13sOCH4YS9OsrrlH08AWwS5//QXY8=";
    };

    nativeBuildInputs = [ pkgs.autoPatchelfHook pkgs.makeWrapper ];

    buildInputs = with unstable; [
      stdenv.cc.cc.lib
    ];

    sourceRoot = "NTRViewer-HR-Linux-x64";

    installPhase = ''
      mkdir -p $out/bin $out/share/ntrviewer-hr
      cp ntrviewer $out/bin/.ntrviewer-hr-unwrapped
      chmod +x $out/bin/.ntrviewer-hr-unwrapped
      # Copy shader presets
      cp -r slang-shaders $out/share/ntrviewer-hr/ || true

      # Wrap with LD_LIBRARY_PATH for dlopen'd libraries (SDL3 video/audio backends)
      makeWrapper $out/bin/.ntrviewer-hr-unwrapped $out/bin/ntrviewer-hr \
        --prefix LD_LIBRARY_PATH : "${unstable.lib.makeLibraryPath ntrviewerRuntimeLibs}"
    '';

    meta = {
      description = "NTR Viewer HR - High resolution 3DS streaming viewer";
      homepage = "https://github.com/xzn/ntrviewer-hr";
      platforms = [ "x86_64-linux" ];
    };
  };

  # NTR-HR 3DS plugin/cia download script
  ntrHrVersion = "0.3.7.0";
  downloadNtrHr = pkgs.writeShellScriptBin "cosmic-ntr-download" ''
    set -euo pipefail

    DEST_DIR="''${1:-$(pwd)/tools/ntr-hr}"
    mkdir -p "$DEST_DIR"

    echo "Downloading NTR-HR v${ntrHrVersion} releases..."

    # BootNTRSelector CIA (main installer)
    echo "  -> BootNTRSelector-PabloMK7-Banner.cia"
    curl -fsSL -o "$DEST_DIR/BootNTRSelector-PabloMK7-Banner.cia" \
      "https://github.com/xzn/ntr-hr/releases/download/v${ntrHrVersion}/BootNTRSelector-PabloMK7-Banner.cia"

    # Mode3 variant (for extended memory games)
    echo "  -> BootNTRSelector-Mode3-PabloMK7-Banner.cia"
    curl -fsSL -o "$DEST_DIR/BootNTRSelector-Mode3-PabloMK7-Banner.cia" \
      "https://github.com/xzn/ntr-hr/releases/download/v${ntrHrVersion}/BootNTRSelector-Mode3-PabloMK7-Banner.cia"

    echo ""
    echo "Downloaded to: $DEST_DIR"
    echo ""
    echo "Install on 3DS:"
    echo "  1. Copy .cia files to SD card"
    echo "  2. Install with FBI or other CIA installer"
    echo "  3. Launch BootNTRSelector from HOME menu"
  '';

  # Deploy NTR-HR to 3DS via FTP
  deployNtrHr = pkgs.writeShellScriptBin "cosmic-ntr-deploy" ''
    set -euo pipefail

    DEST_DIR="''${1:-$(pwd)/tools/ntr-hr}"
    DS_IP="''${2:-10.0.0.222}"
    DS_PORT="''${3:-5000}"

    if [[ ! -f "$DEST_DIR/BootNTRSelector-PabloMK7-Banner.cia" ]]; then
      echo "ERROR: NTR-HR not downloaded. Run cosmic-ntr-download first."
      exit 1
    fi

    echo "Deploying NTR-HR to 3DS at $DS_IP:$DS_PORT..."
    echo "Make sure FTPD is running on your 3DS!"

    # Use netcat to check if FTP is available
    if ! timeout 5 nc -z "$DS_IP" "$DS_PORT" 2>/dev/null; then
      echo "ERROR: Cannot connect to $DS_IP:$DS_PORT"
      echo "Make sure FTPD is running on your 3DS."
      exit 1
    fi

    # Upload via curl FTP to SD card root
    for cia in "$DEST_DIR"/*.cia; do
      filename=$(basename "$cia")
      echo "  -> Uploading $filename"
      curl -fsST "$cia" "ftp://$DS_IP:$DS_PORT/$filename"
    done

    echo ""
    echo "Upload complete! CIAs are on SD card root."
    echo "Install using FBI: SD -> select .cia -> Install and delete CIA"
  '';

in
{
  buildInputs = [
    pythonEnv
    ntrviewerHr
    downloadNtrHr
    deployNtrHr
    # Screen capture dependencies
    unstable.ffmpeg-full
    unstable.xdotool
    unstable.grim
    # GTK for file dialogs
    unstable.gsettings-desktop-schemas
    unstable.gtk3
    # GStreamer + PipeWire for portal-based screen capture
    unstable.gst_all_1.gstreamer
    unstable.gst_all_1.gst-plugins-base
    unstable.gst_all_1.gst-plugins-good
    unstable.pipewire
    # Vulkan runtime for ntrviewer-hr
    unstable.vulkan-loader
  ];

  shellHook = ''
    # GTK schemas needed by file chooser dialogs
    export XDG_DATA_DIRS="${unstable.gsettings-desktop-schemas}/share/gsettings-schemas/${unstable.gsettings-desktop-schemas.name}:${unstable.gtk3}/share/gsettings-schemas/${unstable.gtk3.name}:$XDG_DATA_DIRS"
    # GStreamer plugin path for PipeWire source
    export GST_PLUGIN_SYSTEM_PATH_1_0="${unstable.gst_all_1.gstreamer}/lib/gstreamer-1.0:${unstable.gst_all_1.gst-plugins-base}/lib/gstreamer-1.0:${unstable.gst_all_1.gst-plugins-good}/lib/gstreamer-1.0:${unstable.pipewire}/lib/gstreamer-1.0:$GST_PLUGIN_SYSTEM_PATH_1_0"
    # GObject introspection typelibs for PyGObject (Gst, GstApp, etc.)
    export GI_TYPELIB_PATH="${unstable.gst_all_1.gstreamer.out}/lib/girepository-1.0:${unstable.gst_all_1.gst-plugins-base}/lib/girepository-1.0:${unstable.glib.out}/lib/girepository-1.0:$GI_TYPELIB_PATH"

    echo "NTR-HR Tools:"
    echo "  ntrviewer-hr              - Run NTRViewer-HR (3DS streaming viewer)"
    echo "  cosmic-ntr-download       - Download NTR-HR CIAs for 3DS"
    echo "  cosmic-ntr-deploy [dir] [ip] [port] - Deploy CIAs to 3DS via FTP"
  '';
}
